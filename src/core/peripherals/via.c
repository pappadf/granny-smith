// SPDX-License-Identifier: MIT
// Copyright (c) pappadf

// via.c
// Implements the VIA (Versatile Interface Adapter) module for Granny Smith.

// ============================================================================
// Includes
// ============================================================================

#include "via.h"
#include "common.h"
#include "cpu.h"
#include "log.h"
#include "object.h"
#include "platform.h"
#include "system.h"
#include "system_config.h"
#include "value.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

// Forward declarations — class descriptors are at the bottom of the file but
// via_init / via_delete reference them.
static const class_desc_t via_class;
static const class_desc_t via_port_a_class;
static const class_desc_t via_port_b_class;

// ============================================================================
// Constants and Macros
// ============================================================================

LOG_USE_CATEGORY_NAME("via");

#define PORT_A 0
#define PORT_B 1

#define TIMER_1 0
#define TIMER_2 1

#define ORB_IRB 0
#define ORA_IRA 1
#define DDRB    2
#define DDRA    3
#define T1C_L   4
#define T1C_H   5
#define T1L_L   6
#define T1L_H   7
#define T2C_L   8
#define T2C_H   9
#define SR      10
#define ACR     11
#define PCR     12
#define IFR     13
#define IER     14
#define ORA     15

// Human-readable register names for logging
static const char *via_reg_names[] = {"ORB/IRB", "ORA/IRA", "DDRB", "DDRA", "T1C_L", "T1C_H", "T1L_L", "T1L_H",
                                      "T2C_L",   "T2C_H",   "SR",   "ACR",  "PCR",   "IFR",   "IER",   "ORA"};

#define IFR_CA2 0x01
#define IFR_CA1 0x02
#define IFR_SR  0x04
#define IFR_CB2 0x08
#define IFR_CB1 0x10
#define IFR_T2  0x20
#define IFR_T1  0x40
#define IFR_SET 0x80

#define MAC_CPU_CLOCK 7833600

// Default timer frequency divisor: CPU clock / freq_factor = VIA φ2 clock (~783 kHz)
#define DEFAULT_FREQ_FACTOR 10

// ============================================================================
// Type Definitions (Private)
// ============================================================================

// Represents a VIA (Versatile Interface Adapter) instance with timers, ports, and shift register
struct via {
    /* Plain-data first */
    uint8_t sr;
    uint8_t acr;
    uint8_t pcr;
    uint8_t ifr;
    uint8_t ier;

    // Shift register state for pending shift-out operation
    bool sr_shift_pending; // true if shift-out in progress
    uint8_t sr_shift_data; // data being shifted out

    struct {
        uint64_t start_timestamp;
        uint16_t start_value;
        uint16_t latch;
        uint16_t counter;
        // True once the timer has been armed, i.e. start_timestamp holds a
        // real arm time.  read_timer used to infer this from
        // `start_timestamp == 0`, which is also a legal arm time: a timer
        // armed on CPU cycle 0 read as "never armed" for good.  This replaces
        // a dead `expired` flag that was written in three places and read
        // nowhere -- what actually stops a one-shot re-firing is that its
        // callback schedules no follow-up event.
        bool started;
    } timers[2];

    struct {
        uint8_t output;
        uint8_t input;
        uint8_t direction;
        // Input latch (ACR bits 0/1).  With latching enabled the input
        // register holds the pin levels sampled at the CA1/CB1 active edge
        // rather than tracking them live -- R6522 "Port A and Port B
        // Operation": "With input latching disabled, IRA will always reflect
        // the levels on the PA pins.  With input latching enabled, IRA will
        // reflect the levels on the PA pins at the time the latching occurred
        // (via CA1)."
        uint8_t latched;
        bool ctrl[2];
    } ports[2];

    /* Pointers and interfaces last */
    struct scheduler *scheduler;
    memory_map_t *memory_map;
    memory_interface_t memory_interface;

    // Per-instance callback routing for output signals and interrupts
    via_output_fn output_cb;
    via_shift_out_fn shift_cb;
    via_irq_fn irq_cb;
    void *cb_context;

    // Optional port-A data hooks (Lisa parallel hard disk); NULL when unused.
    via_porta_read_fn porta_read;
    via_porta_write_fn porta_write;
    void *porta_ctx;

    // CPU-to-VIA clock divisor: CPU_clock / freq_factor ≈ 783 kHz VIA φ2 clock
    uint8_t freq_factor;

    // Exact-rational φ2 derivation: φ2 ticks = cycles * ff_num / ff_den.
    // The legacy integer-divisor machines run as (1, freq_factor) — bit-for-
    // bit the historical arithmetic — while machines whose CPU clock is not
    // an integer multiple of 783,360 Hz (PDM: 60/66/80 MHz) install the
    // reduced 783360/cpu_hz rational via via_set_exact_clock() so the
    // guest-visible timer rate is exactly φ2-equivalent over any interval
    // (the PDM dossier's hard constraint).  Config-derived, not part of the
    // checkpointed plain-data prefix; re-derived on every init.
    uint32_t ff_num, ff_den;

    // Object-tree binding — lifetime tied to via_init / via_delete.
    struct object *object;
    struct object *port_a_object;
    struct object *port_b_object;
};

// ============================================================================
// Static Helpers
// ============================================================================

// Convert CPU cycles to VIA timer cycles using the per-instance rational.
// Split division keeps the intermediate product inside 64 bits for any
// cycle count (the ppc_ticks_now precedent).
static uint64_t cpu_to_via_cycles(const via_t *via, uint64_t scheduler_cpu_cycles) {
    uint64_t c = scheduler_cpu_cycles;
    return (c / via->ff_den) * via->ff_num + (c % via->ff_den) * via->ff_num / via->ff_den;
}

// CPU-cycle delay after which `phi2_ticks` VIA clocks have elapsed: the
// smallest d with (d * ff_num) / ff_den >= phi2_ticks, keeping event expiry
// consistent with cpu_to_via_cycles' floor division.
static uint64_t via_cycles_to_cpu(via_t *via, uint64_t phi2_ticks) {
    return (phi2_ticks * via->ff_den + via->ff_num - 1) / via->ff_num;
}

// Update the interrupt flag register and invoke IRQ callback if aggregate changes
static void update_ifr(via_t *restrict via, uint8_t new_ifr) {
    // Bit 7 of IFR is the aggregate "any-enabled-flag-set" bit: set if any of
    // the source-flag bits (bits 6:0) that are enabled in IER are also set in
    // the candidate IFR value. The `& 0x7F` strips any incoming bit 7 so the
    // aggregate is recomputed from the source flags alone.
    uint8_t flags = new_ifr & 0x7F;
    new_ifr = (flags & via->ier) ? (flags | 0x80) : flags;

    if ((via->ifr ^ new_ifr) & 0x80) {
        via->irq_cb(via->cb_context, new_ifr >> 7 & 1);
    }

    via->ifr = new_ifr;
}

// Read the current value of a VIA timer counter (accounting for elapsed time)
static uint16_t read_timer(const via_t *restrict via, int timer) {
    // Never armed: the stored counter is all there is to report.
    if (!via->timers[timer].started)
        return via->timers[timer].counter;

    // Timer is running - calculate current counter value with proper wraparound.
    // The 16-bit counter continuously decrements at VIA clock rate, wrapping from
    // 0x0000 to 0xFFFF. Per R6522 spec: "the counter will continue to decrement"
    // after timeout.
    uint64_t now = scheduler_cpu_cycles(via->scheduler);
    GS_ASSERT(now >= via->timers[timer].start_timestamp);
    uint64_t delta = cpu_to_via_cycles(via, now - via->timers[timer].start_timestamp);

    // Both operands are unsigned, so the subtraction wraps mod 2^N and the
    // (uint16_t) cast keeps the low 16 bits — i.e. the correct mod-2^16 counter
    // value even for arbitrarily large `delta`. (e.g., 0 - 100 = 0xFF9C.)
    return (uint16_t)(via->timers[timer].start_value - delta);
}

// Arm a VIA timer with the specified counter value and callback
static void arm_timer(via_t *restrict via, int timer, uint16_t counter, event_callback_t cb) {
    // Cancel any existing event
    remove_event(via->scheduler, cb, via);

    LOG(2, "arm_timer: timer=%d counter=0x%04x", timer, counter);

    via->timers[timer].start_value = counter;
    via->timers[timer].counter = counter;
    via->timers[timer].start_timestamp = scheduler_cpu_cycles(via->scheduler);
    via->timers[timer].started = true;

    // Timer interrupt fires when the counter wraps around, i.e. delay is counter + 1.
    // Promote to uint64_t before the multiply so an exotic int-width host can't
    // sign-overflow the intermediate.
    scheduler_new_cpu_event(via->scheduler, cb, via, 0, via_cycles_to_cpu(via, (uint64_t)counter + 1), 0);
}

// Shift register completion callback - fires after 8 clock cycles
static void sr_shift_complete_callback(void *source, uint64_t data) {
    via_t *via = (via_t *)source;

    // Only complete the shift if still pending (not cancelled by ACR change)
    if (!via->sr_shift_pending) {
        LOG(2, "sr_shift_complete_callback: shift was cancelled, ignoring");
        return;
    }

    via->sr_shift_pending = false;

    // Deliver the byte via the per-instance shift-out callback.  A machine
    // with nothing to route it to registers NULL -- mcu.c already does for
    // VIA2 -- so this is a live NULL, not a hypothetical one.
    if (via->shift_cb)
        via->shift_cb(via->cb_context, via->sr_shift_data);

    // Signal shift completion to the ROM.  On the Mac Plus the ROM relies
    // on IFR_SR from this callback (the 80-cycle timer IS the shift
    // completion source) to know the command byte has been sent before
    // switching to mode 3 for the keyboard response.  On the SE/30 the
    // ADB module cancels this callback via via_cancel_pending_shift()
    // before it fires, so IFR_SR is only set by the ADB transceiver's own
    // timing (via_input_sr) and no spurious interrupt occurs.
    update_ifr(via, via->ifr | IFR_SR);
}

// Timer 1 timeout callback - handles one-shot and free-running modes
static void t1_callback(void *source, uint64_t data) {

    via_t *via = (via_t *)source;

    GS_ASSERT(via->timers[TIMER_1].started);

    LOG(1, "t1_callback: acr=0x%02x mode=%u latch=0x%04x start_value=0x%04x", via->acr, (unsigned)(via->acr >> 6),
        via->timers[TIMER_1].latch, via->timers[TIMER_1].start_value);
    LOG(2, "t1_callback: IFR will be set to 0x%02x", (unsigned)(via->ifr | IFR_T1));

    switch (via->acr >> 6) {
    case 0: // One-shot
        // Per R6522 "Timer 1 One-Shot Mode": "When the counter reaches zero,
        // the T1 interrupt flag will be set... At this time the counter will
        // continue to decrement at system clock rate.  This allows the system
        // processor to read the contents of the counter to determine the time
        // since interrupt."  So leave start_timestamp standing and let
        // read_timer keep deriving the wrapped value -- exactly what T2 does
        // below.  Scheduling no follow-up event is what stops the flag being
        // set a second time, as the same passage requires.  This used to zero
        // start_timestamp and freeze counter at 0xFFFF, which short-circuited
        // read_timer and returned that constant forever, defeating the one use
        // the datasheet names for the running counter.
        break;
    case 1: // Free‑run
        arm_timer(via, TIMER_1, via->timers[TIMER_1].latch, &t1_callback);
        break;
    case 2: // One-shot w/ PB7 output
        // DDRB bit 7 must be set for PB7 to function as a timer output
        if (via->ports[PORT_B].direction & 0x80)
            via->ports[PORT_B].output |= 0x80; // PB7 is set high when the timer expires
        break;
    case 3: // Free‑run w/ PB7 output
        arm_timer(via, TIMER_1, via->timers[TIMER_1].latch, &t1_callback);
        // DDRB bit 7 must be set for PB7 to function as a timer output
        if (via->ports[PORT_B].direction & 0x80)
            via->ports[PORT_B].output ^= 0x80; // PB7 toggles on each timeout
        break;
    default:
        GS_ASSERT(0);
    }

    update_ifr(via, via->ifr | IFR_T1);
}

// Timer 2 timeout callback - handles one-shot interval timing
static void t2_callback(void *source, uint64_t data) {

    via_t *via = (via_t *)source;

    GS_ASSERT(via->timers[TIMER_2].started);

    LOG(2, "t2_callback: IFR will be set to 0x%02x", (unsigned)(via->ifr | IFR_T2));
    LOG(1, "t2_callback: timer2 expired latch=0x%04x", via->timers[TIMER_2].latch);

    // Per R6522 spec: "After timing out, the counter will continue to decrement.
    // However, setting of the interrupt flag is disabled after initial time-out
    // so that it will not be set by the counter decrementing again through zero."
    //
    // The timer is NOT stopped -- it keeps running.  What prevents the flag
    // being set again is that this callback schedules no follow-up event, so
    // nothing fires when the counter wraps through zero a second time.

    update_ifr(via, via->ifr | IFR_T2);
}

// Write to Timer 1 counter high byte - starts the timer
static void set_t1c_high(via_t *restrict via, uint8_t value) {
    update_ifr(via, via->ifr & ~IFR_T1);

    // Update the latch value for T1C-H
    via->timers[TIMER_1].latch = (via->timers[TIMER_1].latch & 0x00FF) | (value << 8);

    switch (via->acr >> 6) {
    case 2: // One-shot w/ PB7 output
        // DDRB bit 7 must be set for PB7 to function as a timer output
        if (via->ports[PORT_B].direction & 0x80)
            via->ports[PORT_B].output &= 0x7F; // PB7 is set low when the timer starts
        __attribute__((fallthrough));
    case 0: // One-shot mode - PB7 disabled
    case 1: // Free-running mode - PB7 disabled
    case 3: // Free‑run w/ PB7 output
        arm_timer(via, TIMER_1, via->timers[TIMER_1].latch, &t1_callback);
        break;
    default:
        GS_ASSERT(0);
    }
}

// Write to Timer 2 counter high byte - starts the timer
static void set_t2c_high(via_t *restrict via, uint8_t value) {
    // Timer 2: write to T2C-H loads high byte into counter and transfers latch low to counter low
    via->timers[TIMER_2].latch = (via->timers[TIMER_2].latch & 0x00FF) | (value << 8);
    uint16_t counter_value = via->timers[TIMER_2].latch;

    update_ifr(via, via->ifr & ~IFR_T2);

    if ((via->acr & 0x20) == 0) { // one-shot interval timer
        arm_timer(via, TIMER_2, counter_value, &t2_callback);
    } else { // pulse counting timer
        via->timers[TIMER_2].counter = counter_value;
    }
}

// True when ACR enables input latching for `port` -- bit 0 is PA, bit 1 is PB
// (R6522 Figure 14).  With it set, the input register holds the levels sampled
// at the CA1/CB1 active edge instead of tracking the pins live.
static bool port_latch_enabled(const via_t *restrict via, int port) {
    return (via->acr & (port ? 0x02u : 0x01u)) != 0;
}

// IFR control-line flags an ORA/ORB access clears for `port`.
// CA1/CB1 always clear.  CA2/CB2 clear too, EXCEPT when the PCR selects one of
// the two "independent interrupt input" modes (field 001 / 011): R6522 Figure
// 29 note -- "if the CA2/CB2 control in the PCR is selected as 'independent'
// interrupt input, then reading or writing the output register ORA/ORB will NOT
// clear the flag bit.  Instead, the bit must be cleared by writing into the
// IFR."  The output modes (field >= 100) clear normally; nothing drives an edge
// onto a pin the VIA itself is driving.
static uint8_t port_access_ifr_clear_mask(const via_t *restrict via, int port) {
    uint8_t mask = port ? IFR_CB1 : IFR_CA1;
    uint8_t mode = (via->pcr >> (port ? 5 : 1)) & 0x07;
    bool independent = mode < 4 && (mode & 0x01); // fields 001 and 011
    if (!independent)
        mask |= port ? IFR_CB2 : IFR_CA2;
    return mask;
}

// Read from a VIA port combining output and input based on data direction.
// Output pins read back the output register on both ports -- the IRA/IRB
// distinction the datasheet draws is about pin loading, which we do not model,
// so the programmed level is what both report.  Input pins read the live pin
// levels, or the latched sample when ACR enables latching for this port.
static uint8_t read_port(via_t *restrict via, int port) {
    update_ifr(via, via->ifr & (uint8_t)~port_access_ifr_clear_mask(via, port));

    uint8_t inputs = port_latch_enabled(via, port) ? via->ports[port].latched : via->ports[port].input;
    return (via->ports[port].output & via->ports[port].direction) | (inputs & ~via->ports[port].direction);
}

// ============================================================================
// Memory Interface
// ============================================================================

// Memory-mapped read handler for VIA registers
static uint8_t via_read_uint8(void *v, uint32_t addr) {
    via_t *via = (via_t *)v;
    uint8_t ret = 0;
    uint8_t rs = (addr >> 9) & 15; // register select

    // VIA is on the upper byte of the 16-bit wide data bus, so the odd
    // halves of its cells are not the chip.  A guest — or a debugger's
    // memory scan — may read one anyway, and that is not the emulator's
    // business to die over: the bus floats and reads back as the last
    // thing on it, which this model has always presented as zero.
    if (addr & 1) {
        LOG(3, "odd-address read at $%08X: the VIA is on the upper byte only", addr);
        return 0;
    }

    // VIA's 4 RS (register select) lines are connected to line 9-12 of the address bus
    switch (rs) {
    case ORB_IRB:
        ret = read_port(via, PORT_B);
        break;

    case ORA_IRA:
        // Register 1: Read Port A WITH handshake — clears the CA1 flag, and CA2
        // unless the PCR selects an independent input (port_access_ifr_clear_mask).
        // The handshaked access pulses CA2/PSTRB, so a hooked device advances to
        // the next byte and drives it onto the input pins.
        if (via->porta_read)
            via->ports[PORT_A].input = via->porta_read(via->porta_ctx, true);
        ret = read_port(via, PORT_A);
        break;

    case DDRB:
        ret = via->ports[PORT_B].direction;
        break;

    case DDRA:
        ret = via->ports[PORT_A].direction;
        break;

    case T1C_L:
        update_ifr(via, via->ifr & ~IFR_T1); // interrupt flag cleared by reading T1C-L
        ret = (uint8_t)read_timer(via, TIMER_1);
        LOG(2, "Read register T1C_L=0x%02x (%s)", ret, via->timers[TIMER_1].started ? "counting" : "stopped");
        break;

    case T1C_H:
        ret = (uint8_t)(read_timer(via, TIMER_1) >> 8);
        LOG(2, "Read register T1C_H=0x%02x (%s)", ret, via->timers[TIMER_1].started ? "counting" : "stopped");
        break;

    case T1L_L:
        // Read low byte of Timer 1 latch
        ret = (uint8_t)(via->timers[TIMER_1].latch);
        break;

    case T1L_H:
        // Read high byte of Timer 1 latch
        ret = (uint8_t)(via->timers[TIMER_1].latch >> 8);
        break;

    case T2C_L:
        update_ifr(via, via->ifr & ~IFR_T2);
        ret = (uint8_t)read_timer(via, TIMER_2);
        LOG(2, "Read register T2C_L=0x%02x (%s)", ret, via->timers[TIMER_2].started ? "counting" : "stopped");
        break;

    case T2C_H:
        ret = (uint8_t)(read_timer(via, TIMER_2) >> 8);
        LOG(2, "Read register T2C_H=0x%02x (%s)", ret, via->timers[TIMER_2].started ? "counting" : "stopped");
        break;

    case SR:
        update_ifr(via, via->ifr & ~IFR_SR);
        ret = via->sr;
        break;

    case ACR:
        ret = via->acr;
        break;

    case PCR:
        ret = via->pcr;
        break;

    case IFR:
        ret = via->ifr;
        break;

    case IER:
        ret = via->ier | 0x80;
        break;

    case ORA:
        // Register 15: Read Port A WITHOUT handshake — no flag clearing.  No
        // CA2/PSTRB pulse, so a hooked device presents a level byte (e.g. the
        // ProFile state byte) without advancing.
        if (via->porta_read)
            via->ports[PORT_A].input = via->porta_read(via->porta_ctx, false);
        ret = (via->ports[PORT_A].output & via->ports[PORT_A].direction) |
              (via->ports[PORT_A].input & ~via->ports[PORT_A].direction);
        break;
    default:
        GS_ASSERT(0);
        ret = 0;
        break;
    }

    // Log non-timer-counter register reads (timer counters have their own logging)
    if (rs != T1C_L && rs != T1C_H && rs != T2C_L && rs != T2C_H)
        LOG(2, "Read register %s=0x%02x", via_reg_names[rs], ret);

    return ret;
}

// Memory-mapped write handler for VIA registers
static void via_write_uint8(void *v, uint32_t addr, uint8_t value) {
    via_t *via = (via_t *)v;
    uint8_t rs = (addr >> 9) & 15; // register select

    // The odd halves of the VIA's cells are not the chip (see the read
    // side): the write goes nowhere rather than taking the emulator down.
    if (addr & 1) {
        LOG(3, "odd-address write at $%08X = $%02X: the VIA is on the upper byte only", addr, value);
        return;
    }

    // VIA's 4 RS (register select)lines are connected to line 9-12 of the address bus
    switch (rs) {
    case ORB_IRB:
        via->ports[PORT_B].output = value;
        via->output_cb(via->cb_context, 1, via->ports[PORT_B].output & via->ports[PORT_B].direction);
        update_ifr(via, via->ifr & (uint8_t)~port_access_ifr_clear_mask(via, PORT_B));
        break;

    case DDRB:
        via->ports[PORT_B].direction = value;
        via->output_cb(via->cb_context, 1, via->ports[PORT_B].output & via->ports[PORT_B].direction);
        break;

    case DDRA:
        via->ports[PORT_A].direction = value;
        via->output_cb(via->cb_context, 0, via->ports[PORT_A].output & via->ports[PORT_A].direction);
        break;

    case T1C_L: {
        uint16_t old_latch = via->timers[TIMER_1].latch;
        via->timers[TIMER_1].latch = (old_latch & 0xFF00) | value;
        LOG(2, "Write register T1C_L=0x%02x (latch 0x%04x->0x%04x, %s)", value, old_latch, via->timers[TIMER_1].latch,
            via->timers[TIMER_1].started ? "counting" : "stopped");
        break;
    }

    case T1C_H: {
        uint16_t old_latch = via->timers[TIMER_1].latch;
        bool was_counting = via->timers[TIMER_1].started;
        update_ifr(via, via->ifr & ~IFR_T1); // interrupt flag cleared by writing T1C-H
        set_t1c_high(via, value);
        LOG(2, "Write register T1C_H=0x%02x (latch 0x%04x->0x%04x, %s->counting)", value, old_latch,
            via->timers[TIMER_1].latch, was_counting ? "counting" : "stopped");
        break;
    }

    case T1L_L:
        via->timers[TIMER_1].latch = (via->timers[TIMER_1].latch & 0xFF00) | (value & 0xFF);
        break;

    case T1L_H:
        via->timers[TIMER_1].latch = (via->timers[TIMER_1].latch & 0xFF) | (value << 8);
        break;

    case T2C_L: {
        uint16_t old_latch = via->timers[TIMER_2].latch;
        via->timers[TIMER_2].latch = (old_latch & 0xFF00) | (value & 0xFF);
        LOG(2, "Write register T2C_L=0x%02x (latch 0x%04x->0x%04x, %s)", value, old_latch, via->timers[TIMER_2].latch,
            via->timers[TIMER_2].started ? "counting" : "stopped");
        break;
    }

    case T2C_H: {
        uint16_t old_latch = via->timers[TIMER_2].latch;
        bool was_counting = via->timers[TIMER_2].started;
        set_t2c_high(via, value);
        LOG(2, "Write register T2C_H=0x%02x (latch 0x%04x->0x%04x, %s->counting)", value, old_latch,
            via->timers[TIMER_2].latch, was_counting ? "counting" : "stopped");
        break;
    }

    case SR:
        via->sr = value;
        update_ifr(via, via->ifr & ~IFR_SR);
        if (via->acr & 0x10) { // shift out (bit 4 set means output mode)
            LOG(2, "via SR write: value=0x%02x (shift out, mode=%u)", value, (via->acr >> 2) & 7);
            // Store data and mark shift as pending - don't deliver until complete
            via->sr_shift_data = value;
            via->sr_shift_pending = true;
            // Cancel any existing shift event and schedule new one
            remove_event(via->scheduler, &sr_shift_complete_callback, via);
            // 8 VIA clock cycles (= 80 CPU cycles) for internal clock modes;
            // external-clock modes complete when the device drives CB1.
            scheduler_new_cpu_event(via->scheduler, &sr_shift_complete_callback, via, 0, via_cycles_to_cpu(via, 8), 0);
        } else {
            LOG(3, "via SR write: value=0x%02x (shift in mode, not sent)", value);
        }
        break;

    case ACR: {
        uint8_t old_sr_mode = (via->acr >> 2) & 7;
        uint8_t new_sr_mode = (value >> 2) & 7;

        via->acr = value;

        // If shift register mode changed and we had a pending shift, cancel it
        if (old_sr_mode != new_sr_mode && via->sr_shift_pending) {
            LOG(2, "via ACR write: SR mode changed %u->%u, cancelling pending shift", old_sr_mode, new_sr_mode);
            via->sr_shift_pending = false;
            remove_event(via->scheduler, &sr_shift_complete_callback, via);
        }
        break;
    }

    case PCR:

        via->pcr = value;
        break;

    case IFR:
        update_ifr(via, via->ifr & ~value & 0x7f);
        break;

    case IER: {
        // if bit 7 is 0 - 1s will clear bits
        // if bit 7 is 1 - 1s will set bits
        // Bit 7 of the written value is the set/clear selector, not data:
        // R6522 Figure 30 -- "if bit 7 of the data placed on the system data
        // bus during this write operation is a 0, each 1 in bits 6 through 0
        // clears the corresponding bit... Selected bits in the IER can be set
        // by writing to the IER with bit 7 in the data word set to a 1."  It
        // is not storage; the register always reads back with bit 7 as 1 (see
        // the IER read case above), so mask it out of what is stored.  Leaving
        // it in did not change interrupt behaviour -- update_ifr masks flags to
        // 0x7F -- but via_get_ier() exposed the polluted value, which is what
        // machine.via1.ier prints.
        via->ier = (value & 0x80) ? (via->ier | (value & 0x7F)) : (via->ier & ~(value & 0x7F));
        update_ifr(via, via->ifr);
        break;
    }

    case ORA_IRA:
        // Register 1: Write Port A WITH handshake — clears the CA1 flag, and CA2
        // unless the PCR selects an independent input (port_access_ifr_clear_mask).
        // Handshaked access pulses CA2/PSTRB → a hooked device latches the byte.
        via->ports[PORT_A].output = value;
        via->output_cb(via->cb_context, 0, via->ports[PORT_A].output & via->ports[PORT_A].direction);
        if (via->porta_write)
            via->porta_write(via->porta_ctx, value, true);
        update_ifr(via, via->ifr & (uint8_t)~port_access_ifr_clear_mask(via, PORT_A));
        break;

    case ORA:
        // Register 15: Write Port A WITHOUT handshake — no flag clearing, no PSTRB.
        via->ports[PORT_A].output = value;
        via->output_cb(via->cb_context, 0, via->ports[PORT_A].output & via->ports[PORT_A].direction);
        if (via->porta_write)
            via->porta_write(via->porta_ctx, value, false);
        break;

    default:
        GS_ASSERT(0);
    }

    // Log non-timer-counter register writes (timer counters have their own logging)
    if (rs != T1C_L && rs != T1C_H && rs != T2C_L && rs != T2C_H)
        LOG(2, "Write register %s=0x%02x", via_reg_names[rs], value);
}

// The VIA is an 8-bit peripheral on the upper byte of the bus, so a wider
// access is not something it answers -- but it is also not something to die
// over.  A guest, a debugger's memory scan, a `memory.peek width=w` or a memory
// logpoint may issue one, and on the Plus and the Lisa the VIA sits directly in
// the memory map where any of those reach it.  Compose from the byte handlers
// the way the RBV does (rbv.c) and log at level 3: the odd byte of each pair
// falls to via_read_uint8's own odd-address path, which is the floating upper
// byte the hardware presents.  These used to be GS_ASSERT(0), which took the
// emulator down on a debugger read -- and contradicted the odd-byte policy this
// same file argues for eleven lines above it.

// 16-bit read: two byte reads, big-endian, VIA on the even (upper) byte.
static uint16_t via_read_uint16(void *via, uint32_t addr) {
    LOG(3, "16-bit read at $%08X: the VIA is 8-bit; composing from byte reads", addr);
    return (uint16_t)((via_read_uint8(via, addr) << 8) | via_read_uint8(via, addr + 1));
}

// 32-bit read: two word reads.
static uint32_t via_read_uint32(void *via, uint32_t addr) {
    LOG(3, "32-bit read at $%08X: the VIA is 8-bit; composing from byte reads", addr);
    return ((uint32_t)via_read_uint16(via, addr) << 16) | via_read_uint16(via, addr + 2);
}

// 16-bit write: two byte writes, big-endian.
static void via_write_uint16(void *via, uint32_t addr, uint16_t value) {
    LOG(3, "16-bit write at $%08X = $%04X: the VIA is 8-bit; splitting into byte writes", addr, value);
    via_write_uint8(via, addr, (uint8_t)(value >> 8));
    via_write_uint8(via, addr + 1, (uint8_t)value);
}

// 32-bit write: two word writes.
static void via_write_uint32(void *via, uint32_t addr, uint32_t value) {
    LOG(3, "32-bit write at $%08X = $%08X: the VIA is 8-bit; splitting into byte writes", addr, value);
    via_write_uint16(via, addr, (uint16_t)(value >> 16));
    via_write_uint16(via, addr + 2, (uint16_t)value);
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Bus /RESET: the VIA is on every Macintosh board's reset net.
//
// R6522 datasheet, "RESET (!RES)": "Reset (!RES) clears all internal registers
// (except T1 and T2 counters and latches, and the Shift Register (SR)).  In
// the !RES condition, all peripheral interface lines (PA and PB) are placed in
// the input state.  Also, the Timers (T1 and T2), SR and interrupt logic are
// disabled from operation."
//
// So: the direction, output, control and interrupt registers clear; the timer
// counters, timer latches and SR keep their values but stop running; and the
// armed scheduler events go with "disabled from operation".
//
// What is NOT cleared, and why: ports[].input and ports[].ctrl hold what the
// BOARD is driving onto the pins.  A reset of this chip does not change what
// a peripheral outside it is asserting, and modelling it as if it did would
// invent edges on the next via_input_c.
//
// Before this existed there was no via_reset at all -- the IER, IFR, ACR, PCR,
// timers and armed events survived every reset path, so a warm restart could
// take an interrupt for a source the new OS had not installed a handler for
// (05-chipsets-irq F-03).
void via_reset(via_t *restrict via) {
    if (!via)
        return;

    // "Timers and SR disabled from operation": stop them, keeping the counter
    // values the datasheet says survive.  Freeze the live value first, since
    // read_timer derives it from the arm timestamp while the timer runs.
    for (int t = 0; t < 2; t++) {
        via->timers[t].counter = read_timer(via, t);
        via->timers[t].started = false;
    }
    // remove_event, NOT scheduler_forget_source: this is a LIVE device that
    // arms its timers again afterwards, and the primitive also drops the
    // event-TYPE registrations, so the next arm trips
    // scheduler_new_cpu_event's "event type not registered" assert.  Exactly
    // the trap that function's own header warns about -- and walking into it
    // is what broke suite-iicx, suite-iici and suite-iifx on the first run of
    // this change.
    remove_event(via->scheduler, &t1_callback, via);
    remove_event(via->scheduler, &t2_callback, via);
    remove_event(via->scheduler, &sr_shift_complete_callback, via);
    via->sr_shift_pending = false;

    // "Clears all internal registers", except the three named above.
    via->ports[PORT_A].direction = 0; // PA to the input state
    via->ports[PORT_B].direction = 0; // PB likewise
    via->ports[PORT_A].output = 0;
    via->ports[PORT_B].output = 0;
    via->ports[PORT_A].latched = 0;
    via->ports[PORT_B].latched = 0;
    via->acr = 0;
    via->pcr = 0;
    via->ier = 0;

    // The IFR last, through update_ifr, so the aggregate bit is recomputed and
    // the IRQ line is dropped if it was asserted.
    update_ifr(via, 0);

    // DELIBERATELY NOT re-driving output_cb here.  With every direction bit
    // clear the VIA drives nothing, but the callback's contract is
    // `output & direction` -- a value, with no way to say "not driving".
    // Publishing 0 would tell the board every line went LOW, and most of
    // these are active-low: VIA2 port A carries the NuBus slot /NMRQ lines on
    // the II family, so a reset would assert every slot interrupt at once.
    // (Measured: doing it breaks suite-iicx.  It is the mirror of the bug
    // via_init's comment describes, where the control lines came up at 0 and
    // an active-low input read that as ASSERTED.)
    //
    // Leaving the board's picture alone until the guest programs the VIA
    // again is the least-wrong model available, and matches the pull-ups:
    // nothing is driving, so the lines sit high, which is what the board
    // already believes.

    LOG(1, "via_reset: registers cleared, timers stopped (counters kept)");
}

// Initialize a new VIA instance with callbacks and optional checkpoint restoration
via_t *via_init(memory_map_t *restrict map, struct scheduler *scheduler, uint8_t freq_factor, const char *name,
                via_output_fn output_cb, via_shift_out_fn shift_cb, via_irq_fn irq_cb, void *cb_context,
                checkpoint_t *checkpoint) {
    via_t *via = (via_t *)malloc(sizeof(via_t));
    if (via == NULL)
        return NULL;

    memset(via, 0, sizeof(via_t));

    via->scheduler = scheduler;
    via->freq_factor = freq_factor ? freq_factor : DEFAULT_FREQ_FACTOR;
    via->ff_num = 1; // legacy integer-divisor arithmetic unless
    via->ff_den = via->freq_factor; // via_set_exact_clock() installs a rational

    // Store per-instance callback routing
    via->output_cb = output_cb;
    via->shift_cb = shift_cb;
    via->irq_cb = irq_cb;
    via->cb_context = cb_context;

    via->memory_interface.read_uint8 = &via_read_uint8;
    via->memory_interface.read_uint16 = &via_read_uint16;
    via->memory_interface.read_uint32 = &via_read_uint32;

    via->memory_interface.write_uint8 = &via_write_uint8;
    via->memory_interface.write_uint16 = &via_write_uint16;
    via->memory_interface.write_uint32 = &via_write_uint32;

    // Both ports idle high, and so do all four control lines.  This is the
    // 6522 at power-on with the pull-ups every Macintosh board fits: nothing
    // is asserting.  It used to be 0xF7 on port A -- the Plus's SCC W/REQ line
    // held low before the SCC leaves reset -- which made one machine's boot
    // condition the family default, and every II-family machine then had to
    // raise VIA2 PA3 back up because there the same pin is a NuBus slot
    // /NMRQ: leaving it low meant slot $C asserted an interrupt forever.
    // plus.c now drives its own W/REQ line (F-50).
    //
    // The four CONTROL lines had the mirror-image bug: they came up at 0,
    // which for an active-low input reads as ASSERTED, so the SE/30, IIcx, IIx
    // and AV each parked CA1/CA2/CB2 high by hand at init.  They idle high on
    // the pull-ups and the first assertion is a falling edge, so starting them
    // high is the faithful model and deletes all four workarounds.
    //
    // It is a behaviour change: the first falling edge now lands where it
    // should instead of one transition late, which moves two capture windows
    // -- suite-iicx/iicx-gc-beep 5427 -> 5423 frames and suite-plus/plus-beep
    // 15432 -> 15116.  Both goldens were re-cut, having been listened to
    // against the originals first: the waveform is the same beep, earlier in
    // the window (HANDOVER S4.4).
    via->ports[0].input = 0xFF;
    via->ports[1].input = 0xFF;
    via->ports[0].ctrl[0] = 1; // CA1
    via->ports[0].ctrl[1] = 1; // CA2
    via->ports[1].ctrl[0] = 1; // CB1
    via->ports[1].ctrl[1] = 1; // CB2

    // Register event types for checkpointing under the per-instance name
    // ("via1", "via2") so multi-VIA machines don't collide.
    const char *event_name = name ? name : "via";
    scheduler_new_event_type(scheduler, event_name, via, "t1", &t1_callback);
    scheduler_new_event_type(scheduler, event_name, via, "t2", &t2_callback);
    scheduler_new_event_type(scheduler, event_name, via, "sr", &sr_shift_complete_callback);

    // Register with memory map if provided (NULL = machine handles registration)
    if (map)
        memory_map_add(map, 0x00E80000, 0x00080000, "via", &via->memory_interface, via);

    // Load from checkpoint if provided
    if (checkpoint) {
        size_t data_size = offsetof(via_t, scheduler);
        system_read_checkpoint_data(checkpoint, via, data_size);

        // After restoring raw register bits, recompute aggregated IFR bit 7 and
        // re-drive the CPU IRQ line based on IER/IFR state.
        update_ifr(via, via->ifr);

        // Re-drive the external outputs so connected devices reflect restored state.
        via->output_cb(via->cb_context, PORT_A, via->ports[PORT_A].output & via->ports[PORT_A].direction);
        via->output_cb(via->cb_context, PORT_B, via->ports[PORT_B].output & via->ports[PORT_B].direction);
        LOG(1, "via_init: restored from checkpoint IFR=0x%02x", (unsigned)via->ifr);
    }

    // Object-tree binding — instance_data is the via_t itself; ports are
    // attached as named children. Both child member tables share the same
    // class descriptors (via_port_a_class / via_port_b_class) — the channel
    // index is encoded in the member's user_data, the VIA identity comes
    // from the object's instance_data.
    via->object = object_new(&via_class, via, name ? name : "via");
    if (via->object) {
        object_set_order(via->object, 40); // via1/via2 tiebreak on attach order
        object_attach(machine_object(), via->object);
        via->port_a_object = object_new(&via_port_a_class, via, "port_a");
        if (via->port_a_object)
            object_attach(via->object, via->port_a_object);
        via->port_b_object = object_new(&via_port_b_class, via, "port_b");
        if (via->port_b_object)
            object_attach(via->object, via->port_b_object);
    }

    return via;
}

// ============================================================================
// Accessors
// ============================================================================

// Return the VIA's memory-mapped I/O interface for machine-level address decode
const memory_interface_t *via_get_memory_interface(via_t *via) {
    return &via->memory_interface;
}

// === M7c — read-only views for the object model =============================

uint8_t via_get_ifr(const via_t *via) {
    return via ? via->ifr : 0;
}
uint8_t via_get_ier(const via_t *via) {
    return via ? via->ier : 0;
}
uint8_t via_get_acr(const via_t *via) {
    return via ? via->acr : 0;
}
uint8_t via_get_pcr(const via_t *via) {
    return via ? via->pcr : 0;
}
uint8_t via_get_sr(const via_t *via) {
    return via ? via->sr : 0;
}
uint8_t via_port_output(const via_t *via, unsigned which) {
    return (via && which < 2) ? via->ports[which].output : 0;
}
uint8_t via_port_input(const via_t *via, unsigned which) {
    return (via && which < 2) ? via->ports[which].input : 0;
}
uint8_t via_port_direction(const via_t *via, unsigned which) {
    return (via && which < 2) ? via->ports[which].direction : 0;
}
// Live timer counter, for the object model.  These used to return
// timers[].counter, which arm_timer sets to the START value and never updates
// while the timer runs -- so a caller saw the reload value, not the count.
uint16_t via_timer_counter(const via_t *via, unsigned which) {
    return (via && which < 2) ? read_timer(via, (int)which) : 0;
}
uint16_t via_timer_latch(const via_t *via, unsigned which) {
    return (via && which < 2) ? via->timers[which].latch : 0;
}
uint8_t via_get_freq_factor(const via_t *via) {
    return via ? via->freq_factor : 0;
}

// Install the exact-rational φ2 derivation for a CPU clock that is not an
// integer multiple of 783,360 Hz.  Reduces 783360/cpu_hz by gcd so the
// split-division conversion keeps every intermediate inside 64 bits.
void via_set_exact_clock(via_t *via, uint32_t cpu_hz) {
    if (!via || !cpu_hz)
        return;
    uint32_t a = VIA_PHI2_HZ, b = cpu_hz;
    while (b) {
        uint32_t t = a % b;
        a = b;
        b = t;
    }
    via->ff_num = VIA_PHI2_HZ / a;
    via->ff_den = cpu_hz / a;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with a VIA instance
void via_delete(via_t *via) {
    if (!via)
        return;
    LOG(1, "via_delete: freeing via");
    // Drop everything the scheduler still holds for this object before any
    // of it is torn down (proposal-scheduler-source-lifetime).
    scheduler_forget_source(via->scheduler, via);

    if (via->port_b_object) {
        object_detach(via->port_b_object);
        object_delete(via->port_b_object);
        via->port_b_object = NULL;
    }
    if (via->port_a_object) {
        object_detach(via->port_a_object);
        object_delete(via->port_a_object);
        via->port_a_object = NULL;
    }
    if (via->object) {
        object_detach(via->object);
        object_delete(via->object);
        via->object = NULL;
    }
    free(via);
}

// ============================================================================
// Lifecycle: Checkpointing
// ============================================================================

// Save VIA state to a checkpoint
void via_checkpoint(via_t *restrict via, checkpoint_t *checkpoint) {
    if (!via || !checkpoint)
        return;
    size_t data_size = offsetof(via_t, scheduler);
    system_write_checkpoint_data(checkpoint, via, data_size);
}

// ============================================================================
// Operations (Public API)
// ============================================================================

// Helper to re-drive outputs after dependent devices are initialized (e.g., floppy)
void via_redrive_outputs(via_t *via) {
    if (!via)
        return;
    LOG(2, "via_redrive_outputs: ORA=0x%02x ORB=0x%02x", via->ports[PORT_A].output & via->ports[PORT_A].direction,
        via->ports[PORT_B].output & via->ports[PORT_B].direction);
    via->output_cb(via->cb_context, PORT_A, via->ports[PORT_A].output & via->ports[PORT_A].direction);
    via->output_cb(via->cb_context, PORT_B, via->ports[PORT_B].output & via->ports[PORT_B].direction);
}

void via_set_porta_hooks(via_t *via, via_porta_read_fn read_fn, via_porta_write_fn write_fn, void *ctx) {
    if (!via)
        return;
    via->porta_read = read_fn;
    via->porta_write = write_fn;
    via->porta_ctx = ctx;
}

// Read the current shift register value
uint8_t via_read_sr(via_t *via) {
    return via->sr;
}

// Cancel any pending shift-out completion callback and clear the pending flag.
// Called by the ADB module when it reads VIA SR directly at CMD/Listen
// transitions, so the generic 80-cycle timer does not fire a spurious IFR_SR.
void via_cancel_pending_shift(via_t *via) {
    if (via->sr_shift_pending) {
        via->sr_shift_pending = false;
        remove_event(via->scheduler, &sr_shift_complete_callback, via);
    }
}

// Set an input pin value on a VIA port
void via_input(via_t *restrict via, int port, int pin, bool value) {
    GS_ASSERT(port < 2);
    GS_ASSERT(pin < 8);

    if (value)
        via->ports[port].input |= 1 << pin;
    else
        via->ports[port].input &= ~(1 << pin);
}

// Shift a byte into the VIA shift register, or signal shift-out completion.
// Called by external devices (keyboard, ADB transceiver) to deliver a byte
// (mode 3: shift-in under external clock) or to signal that the external
// device has finished clocking all 8 bits of a shift-out (mode 7: shift-out
// under external clock).  On real 6522 hardware, CB1 edges from the external
// device set IFR_SR after 8 clocks regardless of shift direction.
void via_input_sr(via_t *restrict via, uint8_t byte) {
    uint8_t sr_mode = (via->acr >> 2) & 7;

    if (sr_mode == 7) {
        // Mode 7: shift out under external clock.  The ADB transceiver (or
        // other external device) has finished clocking all 8 bits via CB1.
        // On real hardware this sets IFR_SR.
        //
        // If a shift-out is still pending, this external clock burst IS its
        // completion — deliver the byte now and drop the fallback timer.
        // Setting IFR_SR without delivering would tell the guest its byte
        // went out while the device never saw it, and the guest's next SR
        // write would overwrite the undelivered byte.  The Cuda's idle
        // acknowledge racing the host's first command byte did exactly that
        // on the Centris 660AV, silently eating the packet-type byte and
        // desynchronising the transport for the rest of the boot.
        if (via->sr_shift_pending) {
            via->sr_shift_pending = false;
            remove_event(via->scheduler, &sr_shift_complete_callback, via);
            LOG(3, "via_input_sr: mode 7 completes pending shift-out 0x%02x", via->sr_shift_data);
            if (via->shift_cb)
                via->shift_cb(via->cb_context, via->sr_shift_data);
        }
        LOG(3, "via_input_sr: mode 7 -> setting IFR_SR (byte=0x%02x, sr=0x%02x)", byte, via->sr);
        update_ifr(via, via->ifr | IFR_SR);
        return;
    }

    if (via->acr & 0x10)
        return; // modes 4-6: shift-out under internal clock — no external input

    if (sr_mode != 3) {
        // VIA not configured for external-clock shift-in — silently drop the byte.
        // This can happen during early boot or after reset before OS configures VIA.
        LOG(1, "via_input_sr: dropping byte 0x%02x - VIA SR not configured (mode=%u, ACR=0x%02x, expected mode 3)",
            byte, sr_mode, via->acr);
        return;
    }

    // Mode 3: shift-in under external clock — store byte and flag interrupt
    via->sr = byte;
    update_ifr(via, via->ifr | IFR_SR);
}

// Set control line input value (CA1, CA2, CB1, CB2)
void via_input_c(via_t *restrict via, int port, int c, bool value) {
    GS_ASSERT(port == 0 || port == 1);
    GS_ASSERT(c == 0 || c == 1);

    if (port == 0 && c == 0) {
        // CA1 interrupt on active edge (PCR bit 0: 0=negative, 1=positive)
        unsigned int old = via->ports[0].ctrl[0];
        via->ports[0].ctrl[0] = value;
        bool pos_edge = via->pcr & 0x01;
        bool active = pos_edge ? (!old && value) : (old && !value);
        if (active) {
            via->ports[0].latched = via->ports[0].input; // ACR bit 0 sample point
            update_ifr(via, via->ifr | IFR_CA1);
        }

    } else if (port == 0 && c == 1) {
        // CA2 control mode (PCR bits 1-3)
        unsigned int old = via->ports[0].ctrl[1];
        via->ports[0].ctrl[1] = value;
        uint8_t mode = (via->pcr >> 1) & 0x07;
        // Modes 0-3 are input; field bit 1 selects the positive edge (R6522
        // Figure 11: 000 neg, 001 independent-neg, 010 pos, 011 independent-pos)
        if (mode < 4) {
            bool pos_edge = mode & 0x02;
            bool active = pos_edge ? (!old && value) : (old && !value);
            if (active)
                update_ifr(via, via->ifr | IFR_CA2);
        }

    } else if (port == 1 && c == 0) {
        // CB1 interrupt on active edge (PCR bit 4: 0=negative, 1=positive)
        unsigned int old = via->ports[1].ctrl[0];
        via->ports[1].ctrl[0] = value;
        bool pos_edge = via->pcr & 0x10;
        bool active = pos_edge ? (!old && value) : (old && !value);
        if (active) {
            via->ports[1].latched = via->ports[1].input; // ACR bit 1 sample point
            update_ifr(via, via->ifr | IFR_CB1);
        }

    } else {
        // CB2 control mode (PCR bits 5-7)
        unsigned int old = via->ports[1].ctrl[1];
        via->ports[1].ctrl[1] = value;
        uint8_t mode = (via->pcr >> 5) & 0x07;
        // Modes 0-3 are input; field bit 1 selects the positive edge (R6522
        // Figure 11: 000 neg, 001 independent-neg, 010 pos, 011 independent-pos)
        if (mode < 4) {
            bool pos_edge = mode & 0x02;
            bool active = pos_edge ? (!old && value) : (old && !value);
            if (active)
                update_ifr(via, via->ifr | IFR_CB2);
        }
    }
}

// === Object-model class descriptors =========================================
//
// Plus has a single `via1`; SE/30 and IIcx add `via2`. Both VIAs share
// the same `via_class` descriptor — instance_data is the via_t* itself,
// and the object's name (set at attach time) distinguishes them.
//
// Port children: instance_data is the parent's via_t*; the port index
// (0 = A, 1 = B) is encoded in each member's user_data slot.

static via_t *via_instance_from(struct object *self) {
    return (via_t *)object_data(self);
}
#define VIA_BYTE_GETTER(NAME, ACC)                                                                                     \
    static value_t via_attr_##NAME(struct object *self, const member_t *m) {                                           \
        (void)m;                                                                                                       \
        via_t *via = via_instance_from(self);                                                                          \
        value_t v = val_uint(1, ACC(via));                                                                             \
        v.flags |= VAL_HEX;                                                                                            \
        return v;                                                                                                      \
    }

VIA_BYTE_GETTER(ifr, via_get_ifr)
VIA_BYTE_GETTER(ier, via_get_ier)
VIA_BYTE_GETTER(acr, via_get_acr)
VIA_BYTE_GETTER(pcr, via_get_pcr)
VIA_BYTE_GETTER(sr, via_get_sr)

static value_t via_attr_freq_factor(struct object *self, const member_t *m) {
    (void)m;
    via_t *via = via_instance_from(self);
    return val_uint(1, via_get_freq_factor(via));
}

// Port child: instance_data is the parent VIA's via_t*; the port index
// (0 = A, 1 = B) lives in the member's user_data.

static unsigned port_index_from_member(const member_t *m) {
    return (unsigned)(uintptr_t)m->attr.user_data;
}

static value_t via_port_attr_output(struct object *self, const member_t *m) {
    via_t *via = via_instance_from(self);
    value_t v = val_uint(1, via_port_output(via, port_index_from_member(m)));
    v.flags |= VAL_HEX;
    return v;
}
static value_t via_port_attr_input(struct object *self, const member_t *m) {
    via_t *via = via_instance_from(self);
    value_t v = val_uint(1, via_port_input(via, port_index_from_member(m)));
    v.flags |= VAL_HEX;
    return v;
}
static value_t via_port_attr_direction(struct object *self, const member_t *m) {
    via_t *via = via_instance_from(self);
    value_t v = val_uint(1, via_port_direction(via, port_index_from_member(m)));
    v.flags |= VAL_HEX;
    return v;
}

// One member table per port index (A=0, B=1). Two tables instead of four
// because instance_data carries the VIA identity now.
#define VIA_PORT_MEMBERS(VAR, PORT)                                                                                    \
    static const member_t VAR[] = {                                                                                    \
        {.kind = M_ATTR,                                                                                               \
         .name = "output",                                                                                             \
         .flags = VAL_RO,                                                                                              \
         .attr = {.type = V_UINT,                                                                                      \
                  .presentation_flags = VAL_HEX,                                                                       \
                  .get = via_port_attr_output,                                                                         \
                  .set = NULL,                                                                                         \
                  .user_data = (void *)(uintptr_t)(PORT)}},                                                            \
        {.kind = M_ATTR,                                                                                               \
         .name = "input",                                                                                              \
         .flags = VAL_RO,                                                                                              \
         .attr = {.type = V_UINT,                                                                                      \
                  .presentation_flags = VAL_HEX,                                                                       \
                  .get = via_port_attr_input,                                                                          \
                  .set = NULL,                                                                                         \
                  .user_data = (void *)(uintptr_t)(PORT)}},                                                            \
        {.kind = M_ATTR,                                                                                               \
         .name = "direction",                                                                                          \
         .flags = VAL_RO,                                                                                              \
         .attr = {.type = V_UINT,                                                                                      \
                  .presentation_flags = VAL_HEX,                                                                       \
                  .get = via_port_attr_direction,                                                                      \
                  .set = NULL,                                                                                         \
                  .user_data = (void *)(uintptr_t)(PORT)}},                                                            \
    }

// clang-format off — macro expands to definitions; no trailing semicolons.
VIA_PORT_MEMBERS(via_port_a_members, 0);
VIA_PORT_MEMBERS(via_port_b_members, 1);
// clang-format on

static const class_desc_t via_port_a_class = {.name = "via_port",
                                              .members = via_port_a_members,
                                              .n_members = sizeof(via_port_a_members) / sizeof(via_port_a_members[0])};
static const class_desc_t via_port_b_class = {.name = "via_port",
                                              .members = via_port_b_members,
                                              .n_members = sizeof(via_port_b_members) / sizeof(via_port_b_members[0])};

// Status-register member table (shared by via1 / via2 via instance_data).
static const member_t via_members[] = {
    {.kind = M_ATTR,
     .name = "ifr",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = via_attr_ifr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "ier",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = via_attr_ier, .set = NULL}},
    {.kind = M_ATTR,
     .name = "acr",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = via_attr_acr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "pcr",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = via_attr_pcr, .set = NULL}},
    {.kind = M_ATTR,
     .name = "sr",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .presentation_flags = VAL_HEX, .get = via_attr_sr, .set = NULL} },
    {.kind = M_ATTR,
     .name = "freq_factor",
     .flags = VAL_RO,
     .attr = {.type = V_UINT, .get = via_attr_freq_factor, .set = NULL}                       },
};

static const class_desc_t via_class = {
    .name = "via",
    .members = via_members,
    .n_members = sizeof(via_members) / sizeof(via_members[0]),
};
