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
#include "platform.h"
#include "system.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

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

#define FREQ_FACTOR 10 // fix

// VIA timers are decremented every 1.2766us (= 783,336Hz), i.e. 1/10 of the Mac Plus CPU frequency

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
        bool expired; // for one-shot modes: true after first timeout (IFR won't be set again)
    } timers[2];

    struct {
        uint8_t output;
        uint8_t input;
        uint8_t direction;
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
};

// ============================================================================
// Static Helpers
// ============================================================================

// Convert CPU cycles to VIA timer cycles (1/10 ratio)
uint64_t cpu_to_via_cycles(uint64_t scheduler_cpu_cycles) {
    return scheduler_cpu_cycles / FREQ_FACTOR;
}

// Update the interrupt flag register and invoke IRQ callback if aggregate changes
static void update_ifr(via_t *restrict via, uint8_t new_ifr) {
    // bit seven is the aggregate of all enabled bits
    new_ifr = new_ifr & 0x7F & via->ier ? new_ifr | 0x80 : new_ifr & 0x7F;

    if ((via->ifr ^ new_ifr) & 0x80)
        via->irq_cb(via->cb_context, new_ifr >> 7 & 1);

    via->ifr = new_ifr;
}

// Read the current value of a VIA timer counter (accounting for elapsed time)
static uint16_t read_timer(via_t *restrict via, int timer) {
    // If the timer is not running, return the stored counter value
    if (via->timers[timer].start_timestamp == 0)
        return via->timers[timer].counter;

    // Timer is running - calculate current counter value with proper wraparound.
    // The 16-bit counter continuously decrements at VIA clock rate, wrapping from
    // 0x0000 to 0xFFFF. Per R6522 spec: "the counter will continue to decrement"
    // after timeout.
    uint64_t now = scheduler_cpu_cycles(via->scheduler);
    GS_ASSERT(now >= via->timers[timer].start_timestamp);
    uint64_t delta = cpu_to_via_cycles(now - via->timers[timer].start_timestamp);

    // Cast to uint16_t handles wraparound naturally (e.g., 0 - 100 = 0xFF9C)
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
    via->timers[timer].expired = false; // reset expired flag on arm

    // timer interrupt fires when the counter wraps around, i.e. delay is counter + 1
    scheduler_new_cpu_event(via->scheduler, cb, via, 0, (counter + 1) * FREQ_FACTOR, 0);
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

    LOG(2, "sr_shift_complete_callback: shift out complete, delivering byte 0x%02x", via->sr_shift_data);

    // Deliver the byte via the per-instance shift-out callback
    via->shift_cb(via->cb_context, via->sr_shift_data);

    // Set the SR interrupt flag
    update_ifr(via, via->ifr | IFR_SR);
}

// Timer 1 timeout callback - handles one-shot and free-running modes
static void t1_callback(void *source, uint64_t data) {

    via_t *via = (via_t *)source;

    GS_ASSERT(via->timers[TIMER_1].start_timestamp != 0);

    LOG(1, "t1_callback: acr=0x%02x mode=%u latch=0x%04x start_value=0x%04x", via->acr, (unsigned)(via->acr >> 6),
        via->timers[TIMER_1].latch, via->timers[TIMER_1].start_value);
    LOG(2, "t1_callback: IFR will be set to 0x%02x", (unsigned)(via->ifr | IFR_T1));

    switch (via->acr >> 6) {
    case 0: // One-shot
        via->timers[TIMER_1].start_timestamp = 0;
        via->timers[TIMER_1].counter = 0xFFFF; // interrupt fired at wraparound
        break;
    case 1: // Free‑run
        arm_timer(via, TIMER_1, via->timers[TIMER_1].latch, &t1_callback);
        break;
    case 2: // One-shot w/ PB7 output
        via->timers[TIMER_1].start_timestamp = 0;
        via->timers[TIMER_1].counter = 0xFFFF;
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

    GS_ASSERT(via->timers[TIMER_2].start_timestamp != 0);

    LOG(2, "t2_callback: IFR will be set to 0x%02x", (unsigned)(via->ifr | IFR_T2));
    LOG(1, "t2_callback: timer2 expired latch=0x%04x", via->timers[TIMER_2].latch);

    // Per R6522 spec: "After timing out, the counter will continue to decrement.
    // However, setting of the interrupt flag is disabled after initial time-out
    // so that it will not be set by the counter decrementing again through zero."
    //
    // We mark the timer as expired but do NOT stop it - it keeps running.
    // The expired flag prevents re-triggering IFR on subsequent wrap-throughs.
    via->timers[TIMER_2].expired = true;

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

// Read from a VIA port combining output and input based on data direction
static uint8_t read_port(via_t *restrict via, int port) {
    update_ifr(via, via->ifr & ~(port ? (IFR_CB1 | IFR_CB2) : (IFR_CA1 | IFR_CA2)));

    return (via->ports[port].output & via->ports[port].direction) |
           (via->ports[port].input & ~via->ports[port].direction);
}

// ============================================================================
// Memory Interface
// ============================================================================

// Memory-mapped read handler for VIA registers
static uint8_t via_read_uint8(void *v, uint32_t addr) {
    via_t *via = (via_t *)v;
    uint8_t ret = 0;
    uint8_t rs = (addr >> 9) & 15; // register select

    // VIA is on the upper byte of the 16-bit wide data bus
    GS_ASSERT(!(addr & 1));

    // VIA's 4 RS (register select) lines are connected to line 9-12 of the address bus
    switch (rs) {
    case ORB_IRB:
        ret = read_port(via, PORT_B);
        break;

    case ORA_IRA:
        // Read Port A without clearing interrupt flags (no handshake side effects)
        ret = (via->ports[PORT_A].output & via->ports[PORT_A].direction) |
              (via->ports[PORT_A].input & ~via->ports[PORT_A].direction);
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
        LOG(2, "Read register T1C_L=0x%02x (%s)", ret, via->timers[TIMER_1].start_timestamp ? "counting" : "stopped");
        break;

    case T1C_H:
        ret = (uint8_t)(read_timer(via, TIMER_1) >> 8);
        LOG(2, "Read register T1C_H=0x%02x (%s)", ret, via->timers[TIMER_1].start_timestamp ? "counting" : "stopped");
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
        LOG(2, "Read register T2C_L=0x%02x (%s)", ret, via->timers[TIMER_2].start_timestamp ? "counting" : "stopped");
        break;

    case T2C_H:
        ret = (uint8_t)(read_timer(via, TIMER_2) >> 8);
        LOG(2, "Read register T2C_H=0x%02x (%s)", ret, via->timers[TIMER_2].start_timestamp ? "counting" : "stopped");
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
        ret = read_port(via, PORT_A);
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

    // VIA is on the upper byte of the 16-bit wide data bus
    GS_ASSERT(!(addr & 1));

    // VIA's 4 RS (register select)lines are connected to line 9-12 of the address bus
    switch (rs) {
    case ORB_IRB:
        via->ports[PORT_B].output = value;
        via->output_cb(via->cb_context, 1, via->ports[PORT_B].output & via->ports[PORT_B].direction);
        update_ifr(via, via->ifr & ~(IFR_CB1 | IFR_CB2));
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
            via->timers[TIMER_1].start_timestamp ? "counting" : "stopped");
        break;
    }

    case T1C_H: {
        uint16_t old_latch = via->timers[TIMER_1].latch;
        bool was_counting = via->timers[TIMER_1].start_timestamp != 0;
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
            via->timers[TIMER_2].start_timestamp ? "counting" : "stopped");
        break;
    }

    case T2C_H: {
        uint16_t old_latch = via->timers[TIMER_2].latch;
        bool was_counting = via->timers[TIMER_2].start_timestamp != 0;
        set_t2c_high(via, value);
        LOG(2, "Write register T2C_H=0x%02x (latch 0x%04x->0x%04x, %s->counting)", value, old_latch,
            via->timers[TIMER_2].latch, was_counting ? "counting" : "stopped");
        break;
    }

    case SR:
        via->sr = value;
        update_ifr(via, via->ifr & ~IFR_SR);
        if (via->acr & 0x10) { // shift out (bit 4 set means output mode)
            LOG(2, "via SR write: value=0x%02x (shift out to keyboard, mode=%u)", value, (via->acr >> 2) & 7);
            // Store data and mark shift as pending - don't deliver until complete
            via->sr_shift_data = value;
            via->sr_shift_pending = true;
            // Cancel any existing shift event and schedule new one
            remove_event(via->scheduler, &sr_shift_complete_callback, via);
            // Schedule completion after 8 clock cycles (8 VIA cycles = 8 * FREQ_FACTOR CPU cycles)
            scheduler_new_cpu_event(via->scheduler, &sr_shift_complete_callback, via, 0, 8 * FREQ_FACTOR, 0);
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

    case IER:
        // if bit 7 is 0 - 1s will clear bits
        // if bit 7 is 1 - 1s will set bits
        via->ier = value & 0x80 ? via->ier | value : via->ier & ~value;
        update_ifr(via, via->ifr);
        break;

    case ORA:
        via->ports[PORT_A].output = value;
        via->output_cb(via->cb_context, 0, via->ports[PORT_A].output & via->ports[PORT_A].direction);
        update_ifr(via, via->ifr & ~(IFR_CA1 | IFR_CA2));
        break;

    default:
        GS_ASSERT(0);
    }

    // Log non-timer-counter register writes (timer counters have their own logging)
    if (rs != T1C_L && rs != T1C_H && rs != T2C_L && rs != T2C_H)
        LOG(2, "Write register %s=0x%02x", via_reg_names[rs], value);
}

// Unimplemented 16-bit read handler (VIA is 8-bit only)
static uint16_t via_read_uint16(void *via, uint32_t addr) {
    (void)addr;
    GS_ASSERT(0);
    return 0;
}

// Unimplemented 32-bit read handler (VIA is 8-bit only)
static uint32_t via_read_uint32(void *via, uint32_t addr) {
    (void)addr;
    GS_ASSERT(0);
    return 0;
}

// Unimplemented 16-bit write handler (VIA is 8-bit only)
static void via_write_uint16(void *via, uint32_t addr, uint16_t value) {
    (void)addr;
    (void)value;
    GS_ASSERT(0);
}

// Unimplemented 32-bit write handler (VIA is 8-bit only)
static void via_write_uint32(void *via, uint32_t addr, uint32_t value) {
    (void)addr;
    (void)value;
    GS_ASSERT(0);
}

// ============================================================================
// Lifecycle: Constructor
// ============================================================================

// Initialize a new VIA instance with callbacks and optional checkpoint restoration
via_t *via_init(memory_map_t *restrict map, struct scheduler *scheduler, via_output_fn output_cb,
                via_shift_out_fn shift_cb, via_irq_fn irq_cb, void *cb_context, checkpoint_t *checkpoint) {
    via_t *via = (via_t *)malloc(sizeof(via_t));
    if (via == NULL)
        return NULL;

    memset(via, 0, sizeof(via_t));

    via->scheduler = scheduler;

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

    via->ports[0].input = 0xf7;
    via->ports[1].input = 0xFF;

    // Register event types for checkpointing
    scheduler_new_event_type(scheduler, "via", via, "t1", &t1_callback);
    scheduler_new_event_type(scheduler, "via", via, "t2", &t2_callback);
    scheduler_new_event_type(scheduler, "via", via, "sr", &sr_shift_complete_callback);

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

    return via;
}

// ============================================================================
// Accessors
// ============================================================================

// Return the VIA's memory-mapped I/O interface for machine-level address decode
const memory_interface_t *via_get_memory_interface(via_t *via) {
    return &via->memory_interface;
}

// ============================================================================
// Lifecycle: Destructor
// ============================================================================

// Free resources associated with a VIA instance
void via_delete(via_t *via) {
    if (!via)
        return;
    LOG(1, "via_delete: freeing via");
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

// Set an input pin value on a VIA port
void via_input(via_t *restrict via, int port, int pin, bool value) {
    GS_ASSERT(port < 2);
    GS_ASSERT(pin < 8);

    if (value)
        via->ports[port].input |= 1 << pin;
    else
        via->ports[port].input &= ~(1 << pin);
}

// Shift a byte into the VIA shift register (keyboard input)
void via_input_sr(via_t *restrict via, uint8_t byte) {
    if (via->acr & 0x10)
        return; // if sr is in output mode...

    // Check if VIA is configured for shift-in mode (external clock)
    uint8_t sr_mode = (via->acr >> 2) & 3;
    if (sr_mode != 3) {
        // VIA not yet configured for keyboard input - silently drop the byte
        // This can happen during early boot or after reset before OS configures VIA
        LOG(1, "via_input_sr: dropping byte 0x%02x - VIA SR not configured (mode=%u, ACR=0x%02x, expected mode 3)",
            byte, sr_mode, via->acr);
        return;
    }

    via->sr = byte;

    // we received data - flag the interrupt
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
        if (active)
            update_ifr(via, via->ifr | IFR_CA1);

    } else if (port == 0 && c == 1) {
        // CA2 control mode (PCR bits 1-3)
        unsigned int old = via->ports[0].ctrl[1];
        via->ports[0].ctrl[1] = value;
        uint8_t mode = (via->pcr >> 1) & 0x07;
        // Modes 0-3 are input; bit 2 selects positive edge
        if (mode < 4) {
            bool pos_edge = mode & 0x04;
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
        if (active)
            update_ifr(via, via->ifr | IFR_CB1);

    } else {
        // CB2 control mode (PCR bits 5-7)
        unsigned int old = via->ports[1].ctrl[1];
        via->ports[1].ctrl[1] = value;
        uint8_t mode = (via->pcr >> 5) & 0x07;
        // Modes 0-3 are input; bit 2 selects positive edge
        if (mode < 4) {
            bool pos_edge = mode & 0x04;
            bool active = pos_edge ? (!old && value) : (old && !value);
            if (active)
                update_ifr(via, via->ifr | IFR_CB2);
        }
    }
}
